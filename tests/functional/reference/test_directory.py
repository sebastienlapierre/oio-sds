import logging
import simplejson as json
from tests.utils import BaseTestCase


class TestDirectoryFunctional(BaseTestCase):

    def test_services_cycle(self):
        params = self.param_srv(self._random_user(), 'echo')
        resp = self.session.post(self._url_ref('create'), params=params)

        resp = self.session.get(self._url_ref('show'), params=params)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIsInstance(body, dict)
        self.assertItemsEqual(body['srv'], [])

        srv0 = self._srv('echo')
        srv1 = self._srv('echo')

        srv0['score'] = 1
        srv1['score'] = 0
        self._flush_cs('echo')
        self._lock_srv(srv0)
        self._lock_srv(srv1)
        self._reload()

        # Initial link
        resp = self.session.post(self._url_ref('link'), params=params)
        self.assertEqual(resp.status_code, 200)

        resp = self.session.get(self._url_ref('show'), params=params)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIsInstance(body, dict)
        logging.debug("Got services %s", repr(body))
        self.assertIn(srv0['addr'], [x['host']
                      for x in body['srv'] if x['type'] == 'echo'])

        # second identical link
        resp = self.session.post(self._url_ref('link'), params=params)
        self.assertEqual(resp.status_code, 200)

        resp = self.session.get(self._url_ref('show'), params=params)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIsInstance(body, dict)
        self.assertEqual(len(body['srv']), 1)
        self.assertEqual(body['srv'][0]['host'], srv0['addr'])

        # XXX JFS: on a srvtype that has no config (e.g. 'echo'), 'Renew'
        # and 'Link' won't append or replace.

        # Force a relink with a 0 score, then relink
        srv0['score'] = 0
        srv1['score'] = 1
        self._flush_cs('echo')
        self._lock_srv(srv0)
        self._lock_srv(srv1)
        self._reload()

        resp = self.session.post(self._url_ref('link'), params=params)
        self.assertEqual(resp.status_code, 200)

        resp = self.session.get(self._url_ref('show'), params=params)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIsInstance(body, dict)
        self.assertEqual(len(body['srv']), 1)
        self.assertEqual(body['srv'][0]['host'], srv0['addr'])

        # unlink
        resp = self.session.post(self._url_ref('unlink'), params=params)
        self.assertEqual(resp.status_code, 204)

        resp = self.session.get(self._url_ref('show'), params=params)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIsInstance(body, dict)
        self.assertItemsEqual(body['srv'], [])

        # Renew while not linked
        resp = self.session.post(self._url_ref('renew'), params=params)
        self.assertEqual(resp.status_code, 200)

        resp = self.session.get(self._url_ref('show'), params=params)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIsInstance(body, dict)
        self.assertEqual(len(body['srv']), 1)
        self.assertEqual(body['srv'][0]['host'], srv1['addr'])

        # Renew while linked
        srv0['score'] = 1
        srv1['score'] = 0
        self._flush_cs('echo')
        self._lock_srv(srv0)
        self._lock_srv(srv1)
        self._reload()

        resp = self.session.post(self._url_ref('renew'), params=params)
        self.assertEqual(resp.status_code, 200)

        resp = self.session.get(self._url_ref('show'), params=params)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIsInstance(body, dict)
        self.assertEqual(len(body['srv']), 1)
        self.assertEqual(body['srv'][0]['host'], srv1['addr'])

        # Force without header while linked
        enforced = {'host': self._addr(), 'type': 'echo',
                    'seq': body['srv'][0]['seq'], 'args': ''}
        resp = self.session.post(self._url_ref('force'), params=params,
                                 data=json.dumps(enforced))
        self.assertEqual(resp.status_code, 403)

        # Force with header while linked
        resp = self.session.post(self._url_ref('force'), params=params,
                                 headers={'X-oio-action-mode': 'replace'},
                                 data=json.dumps(enforced))
        self.assertEqual(resp.status_code, 204)

        # unlink
        resp = self.session.post(self._url_ref('unlink'), params=params)
        self.assertEqual(resp.status_code, 204)

        # Force without header while not linked
        resp = self.session.post(self._url_ref('force'), params=params,
                                 data=json.dumps(enforced))
        self.assertEqual(resp.status_code, 204)

        # unlink
        resp = self.session.post(self._url_ref('unlink'), params=params)
        self.assertEqual(resp.status_code, 204)

        # Force with header while not linked
        resp = self.session.post(self._url_ref('force'), params=params,
                                 headers={'X-oio-action-mode': 'replace'},
                                 data=json.dumps(enforced))
        self.assertEqual(resp.status_code, 204)
